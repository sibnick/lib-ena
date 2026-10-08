#!/usr/bin/env python3
"""
Deploy the nginx sample (Unikraft + native AWS ENA driver) to Amazon EC2.

The script builds a GRUB-bootable raw disk image, uploads it to an EBS
snapshot with the EBS Direct API, registers an AMI with ENA support, and
launches a 1 vCPU instance. It then reads the serial console to confirm
that the ENA driver came up and the network stack has an address.

Required environment variables:
    SUBNET_ID    - EC2 subnet to launch the instance into

Optional environment variables:
    AWS_REGION    - defaults to us-east-1
    INSTANCE_TYPE - defaults to t3.micro (1 vCPU)
    PRIVATE_IP    - private IPv4 to assign to the instance
    GATEWAY_IP    - VPC gateway, used in the kernel command line
    NETMASK       - VPC netmask, used in the kernel command line
    SG_ID         - security group with TCP port 80 open. The script
                    creates 'unikraft-nginx-sg' when this is not set.

Without PRIVATE_IP the unikernel configures the interface with DHCP.
"""

import json
import os
import re
import sys
import time
import base64
import hashlib
import struct
import subprocess
from pathlib import Path

SAMPLE_DIR = Path(__file__).resolve().parent.parent

AWS_REGION = os.environ.get("AWS_REGION", "us-east-1")
INSTANCE_TYPE = os.environ.get("INSTANCE_TYPE", "t3.micro")
SUBNET_ID = os.environ.get("SUBNET_ID", "")
PRIVATE_IP = os.environ.get("PRIVATE_IP", "")
GATEWAY_IP = os.environ.get("GATEWAY_IP", "")
NETMASK = os.environ.get("NETMASK", "")
SG_ID = os.environ.get("SG_ID", "")
INSTANCE_NAME = "unikraft-nginx-server"
BLOCK_SIZE = 524288  # 512 KiB EBS direct block size


def run_cmd(cmd, check=True, capture=True, cwd=None, env=None):
    print(f"[CMD] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(
        cmd,
        shell=isinstance(cmd, str),
        check=check,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        cwd=cwd,
        env=env,
    )
    return res.stdout.strip() if capture and res.stdout else ""


def build_unikraft():
    print("==================================================")
    print("Step 1: Building the rootfs and the unikernel...")
    print("==================================================")
    run_cmd(["./scripts/build_rootfs.sh"], cwd=str(SAMPLE_DIR))
    run_cmd(
        ["kraft", "build", "--target", "aws-t3-x86_64", "--no-prompt"],
        cwd=str(SAMPLE_DIR),
    )

    search_dirs = [SAMPLE_DIR / "build", SAMPLE_DIR / ".unikraft" / "build"]
    kernels = [
        p
        for d in search_dirs
        for p in d.glob("*_qemu-x86_64")
        if not p.name.endswith(".dbg") and not p.name.endswith(".cmd")
    ]
    if not kernels:
        raise RuntimeError("Kernel binary not found in build/ or .unikraft/build/")
    kernel_path = sorted(kernels)[0]
    print(f"[SUCCESS] Kernel built: {kernel_path} ({kernel_path.stat().st_size} bytes)")
    return kernel_path


def create_bootable_disk(kernel_path):
    print("==================================================")
    print("Step 2: Creating bootable disk image with GRUB...")
    print("==================================================")
    env = dict(os.environ)
    env["KERNEL_BIN"] = str(kernel_path)
    if PRIVATE_IP:
        if not NETMASK:
            raise RuntimeError("NETMASK is required when PRIVATE_IP is set")
        cidr = (
            sum(bin(int(x)).count("1") for x in NETMASK.split("."))
            if "." in NETMASK
            else NETMASK
        )
        env["NETDEV_IP"] = f"{PRIVATE_IP}/{cidr}"
        if GATEWAY_IP:
            env["NETDEV_GW"] = GATEWAY_IP
    else:
        print("[INFO] No PRIVATE_IP given. The unikernel uses DHCP.")
    run_cmd(["./scripts/build_disk.sh"], cwd=str(SAMPLE_DIR), env=env)

    disk_raw = SAMPLE_DIR / "build" / "disk.raw"
    print(
        f"[SUCCESS] Bootable disk image ready at {disk_raw} ({disk_raw.stat().st_size} bytes)"
    )
    return disk_raw


def upload_ebs_snapshot(disk_raw):
    print("==================================================")
    print("Step 3: Uploading disk to an EBS snapshot...")
    print("==================================================")
    desc = f"unikraft-nginx-{int(time.time())}"
    start_out = run_cmd(
        [
            "aws",
            "ebs",
            "start-snapshot",
            "--volume-size",
            "1",
            "--description",
            desc,
            "--region",
            AWS_REGION,
            "--output",
            "json",
        ]
    )
    snapshot_id = json.loads(start_out)["SnapshotId"]
    print(f"[INFO] Started EBS snapshot: {snapshot_id}")

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

            csum_b64 = base64.b64encode(hashlib.sha256(data).digest()).decode("ascii")
            block_file = SAMPLE_DIR / "build" / f"block_{block_idx}.bin"
            block_file.write_bytes(data)

            run_cmd(
                [
                    "aws",
                    "ebs",
                    "put-snapshot-block",
                    "--snapshot-id",
                    snapshot_id,
                    "--block-index",
                    str(block_idx),
                    "--block-data",
                    str(block_file),
                    "--data-length",
                    str(BLOCK_SIZE),
                    "--checksum",
                    csum_b64,
                    "--checksum-algorithm",
                    "SHA256",
                    "--region",
                    AWS_REGION,
                    "--output",
                    "json",
                ]
            )
            block_file.unlink()
            changed_blocks += 1
            print(
                f"[INFO] Uploaded block {block_idx} ({changed_blocks} non-empty blocks)"
            )

    run_cmd(
        [
            "aws",
            "ebs",
            "complete-snapshot",
            "--snapshot-id",
            snapshot_id,
            "--changed-blocks-count",
            str(changed_blocks),
            "--region",
            AWS_REGION,
            "--output",
            "json",
        ]
    )

    print(f"[INFO] Waiting for snapshot {snapshot_id} to complete...")
    while True:
        state = run_cmd(
            [
                "aws",
                "ec2",
                "describe-snapshots",
                "--snapshot-ids",
                snapshot_id,
                "--region",
                AWS_REGION,
                "--query",
                "Snapshots[0].State",
                "--output",
                "text",
            ]
        )
        if state == "completed":
            print(f"[SUCCESS] Snapshot {snapshot_id} is completed.")
            break
        print(f"[INFO] Snapshot state: {state}, waiting 3s...")
        time.sleep(3)

    return snapshot_id


def register_ami(snapshot_id):
    print("==================================================")
    print("Step 4: Registering an ENA-enabled AMI...")
    print("==================================================")
    ami_name = f"unikraft-nginx-{int(time.time())}"
    block_device_mapping = [
        {
            "DeviceName": "/dev/sda1",
            "Ebs": {
                "SnapshotId": snapshot_id,
                "VolumeSize": 1,
                "VolumeType": "gp3",
                "DeleteOnTermination": True,
            },
        }
    ]

    reg_out = run_cmd(
        [
            "aws",
            "ec2",
            "register-image",
            "--name",
            ami_name,
            "--description",
            "Unikraft nginx web server with native AWS ENA driver",
            "--architecture",
            "x86_64",
            "--virtualization-type",
            "hvm",
            "--root-device-name",
            "/dev/sda1",
            "--ena-support",
            "--block-device-mappings",
            json.dumps(block_device_mapping),
            "--region",
            AWS_REGION,
            "--output",
            "json",
        ]
    )
    ami_id = json.loads(reg_out)["ImageId"]
    print(f"[SUCCESS] AMI registered: {ami_id} ({ami_name})")

    while True:
        state = run_cmd(
            [
                "aws",
                "ec2",
                "describe-images",
                "--image-ids",
                ami_id,
                "--region",
                AWS_REGION,
                "--query",
                "Images[0].State",
                "--output",
                "text",
            ]
        )
        if state == "available":
            break
        print(f"[INFO] AMI state: {state}, waiting 3s...")
        time.sleep(3)

    ena = run_cmd(
        [
            "aws",
            "ec2",
            "describe-images",
            "--image-ids",
            ami_id,
            "--region",
            AWS_REGION,
            "--query",
            "Images[0].EnaSupport",
            "--output",
            "text",
        ]
    )
    if ena != "True":
        raise RuntimeError(
            f"AMI {ami_id} does not report ENA support (EnaSupport={ena})"
        )
    print(f"[SUCCESS] AMI {ami_id} is available with EnaSupport=true.")
    return ami_id


def setup_security_group():
    print("==================================================")
    print("Step 5: Configuring the security group (TCP 80)...")
    print("==================================================")
    if SG_ID:
        print(f"[INFO] Using security group from SG_ID: {SG_ID}")
        return SG_ID

    sg_name = "unikraft-nginx-sg"
    sgs = json.loads(
        run_cmd(
            [
                "aws",
                "ec2",
                "describe-security-groups",
                "--filters",
                f"Name=group-name,Values={sg_name}",
                "--region",
                AWS_REGION,
                "--output",
                "json",
            ]
        )
    ).get("SecurityGroups", [])
    if sgs:
        sg_id = sgs[0]["GroupId"]
        print(f"[INFO] Found existing security group: {sg_id}")
        return sg_id

    vpc_id = run_cmd(
        [
            "aws",
            "ec2",
            "describe-vpcs",
            "--filters",
            "Name=isDefault,Values=true",
            "--region",
            AWS_REGION,
            "--query",
            "Vpcs[0].VpcId",
            "--output",
            "text",
        ]
    )
    sg_id = json.loads(
        run_cmd(
            [
                "aws",
                "ec2",
                "create-security-group",
                "--group-name",
                sg_name,
                "--description",
                "Security group for the Unikraft nginx sample",
                "--vpc-id",
                vpc_id,
                "--region",
                AWS_REGION,
                "--output",
                "json",
            ]
        )
    )["GroupId"]
    run_cmd(
        [
            "aws",
            "ec2",
            "authorize-security-group-ingress",
            "--group-id",
            sg_id,
            "--protocol",
            "tcp",
            "--port",
            "80",
            "--cidr",
            "0.0.0.0/0",
            "--region",
            AWS_REGION,
        ]
    )
    print(f"[INFO] Created security group with TCP 80 open: {sg_id}")
    return sg_id


def launch_ec2_instance(ami_id, sg_id):
    print("==================================================")
    print(f"Step 6: Launching {INSTANCE_TYPE} instance...")
    print("==================================================")
    old_ids = json.loads(
        run_cmd(
            [
                "aws",
                "ec2",
                "describe-instances",
                "--filters",
                f"Name=tag:Name,Values={INSTANCE_NAME}",
                "Name=instance-state-name,Values=running,pending",
                "--query",
                "Reservations[].Instances[].InstanceId",
                "--region",
                AWS_REGION,
                "--output",
                "json",
            ]
        )
    )
    if old_ids:
        print(f"[INFO] Terminating {len(old_ids)} existing instance(s): {old_ids}")
        run_cmd(
            ["aws", "ec2", "terminate-instances", "--instance-ids"]
            + old_ids
            + ["--region", AWS_REGION]
        )
        while True:
            states = json.loads(
                run_cmd(
                    ["aws", "ec2", "describe-instances", "--instance-ids"]
                    + old_ids
                    + [
                        "--query",
                        "Reservations[].Instances[].State.Name",
                        "--region",
                        AWS_REGION,
                        "--output",
                        "json",
                    ]
                )
            )
            if all(s == "terminated" for s in states):
                break
            time.sleep(2)

    cmd = [
        "aws",
        "ec2",
        "run-instances",
        "--image-id",
        ami_id,
        "--instance-type",
        INSTANCE_TYPE,
        "--security-group-ids",
        sg_id,
        "--subnet-id",
        SUBNET_ID,
        "--count",
        "1",
        "--tag-specifications",
        f"ResourceType=instance,Tags=[{{Key=Name,Value={INSTANCE_NAME}}}]",
        "--region",
        AWS_REGION,
        "--output",
        "json",
    ]
    if PRIVATE_IP:
        cmd += ["--private-ip-address", PRIVATE_IP]
    instance_id = json.loads(run_cmd(cmd))["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched instance: {instance_id}")

    print("[INFO] Waiting for the instance to become 'running'...")
    while True:
        inst = json.loads(
            run_cmd(
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
        )["Reservations"][0]["Instances"][0]
        state = inst["State"]["Name"]
        public_ip = inst.get("PublicIpAddress", "")
        if state == "running":
            private_ip = inst["PrivateIpAddress"]
            eni = inst["NetworkInterfaces"][0]
            print(
                f"[INFO] ENA interface: {eni['NetworkInterfaceId']} "
                f"type={eni['InterfaceType']} description={eni.get('Description', '')}"
            )
            print(
                f"[SUCCESS] Instance is RUNNING. Private IP: {private_ip}, Public IP: {public_ip}"
            )
            return instance_id, private_ip, public_ip
        print(f"[INFO] Instance state: {state}, waiting 3s...")
        time.sleep(3)


def check_console(instance_id):
    """Read the serial console and report the ENA and address state."""
    print("==================================================")
    print("Step 7: Reading the serial console...")
    print("==================================================")
    deadline = time.time() + 180
    text = ""
    while time.time() < deadline:
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
        text = json.loads(out).get("Output", "")
        if "Set IPv4 address" in text or "Interface is up" in text:
            break
        time.sleep(5)

    console_path = SAMPLE_DIR / f"console_{int(time.time())}.txt"
    console_path.write_text(text)
    print(f"[INFO] Console output saved to {console_path} ({len(text)} bytes)")

    checks = [
        # The netdev name in brackets comes from the driver: 'ena' on EC2.
        ("ENA network device registered", r"Registered netdev\d+.*\(ena\)"),
        ("Network interface up", r"Interface is up"),
        (
            "IPv4 address assigned",
            r"Set IPv4 address \d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}",
        ),
        ("Initrd unpacked", r"mounting ramfs at /"),
    ]
    ok = True
    for label, pattern in checks:
        if re.search(pattern, text, re.IGNORECASE):
            print(f"[OK]   {label} found in console output.")
        else:
            print(f"[WARN] {label} not found in console output.")
            ok = False
    return ok, text


def main():
    if not SUBNET_ID:
        print("[ERR] SUBNET_ID is required.")
        print("[ERR] Example: SUBNET_ID=subnet-xxx python3 scripts/deploy_aws.py")
        sys.exit(1)

    start_time = time.time()
    kernel_path = build_unikraft()
    disk_raw = create_bootable_disk(kernel_path)
    snapshot_id = upload_ebs_snapshot(disk_raw)
    ami_id = register_ami(snapshot_id)
    sg_id = setup_security_group()
    instance_id, private_ip, public_ip = launch_ec2_instance(ami_id, sg_id)
    console_ok, _ = check_console(instance_id)

    print("==================================================")
    print("DEPLOYMENT COMPLETE" if console_ok else "DEPLOYED WITH WARNINGS")
    print("==================================================")
    print(f"Instance ID : {instance_id}")
    print(f"Instance typ: {INSTANCE_TYPE}")
    print(f"Private IPv4: {private_ip}")
    print(f"Public IPv4 : {public_ip}")
    print(f"AMI ID      : {ami_id}")
    print(f"Snapshot ID : {snapshot_id}")
    print(f"Elapsed     : {time.time() - start_time:.1f} s")
    print("\nRun the smoke test with:")
    print(f"  ./scripts/smoke_test.sh {private_ip or public_ip}")
    print("==================================================")


if __name__ == "__main__":
    main()
