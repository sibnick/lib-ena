# AWS EC2 Deployment Guide for Unikraft ENA

## 1. Overview

This document describes how to build, package, and deploy a Unikraft unikernel with the native ENA driver on Amazon EC2.

---

## 2. Prerequisites

Before you start deployment, make sure you have the following items:

- An active AWS account.
- AWS CLI installed and configured with appropriate IAM permissions.
- KraftKit build tool installed on your development system.
- An Amazon S3 bucket in your target AWS region for VM import.
- A VM Import service role (`vmimport`) configured in your AWS account.

---

## 3. Build the Unikraft Image

Build the KVM image for x86_64 or ARM64 architecture:

```bash
kraft build --target kvm --plat qemu --arch x86_64
```

This step creates a raw kernel image in the build directory.

---

## 4. Package and Create the AMI

Follow these steps to convert the Unikraft image into an Amazon Machine Image (AMI):

### 4.1 Convert Binary to Raw Disk Image

Create a raw disk image that contains the Unikraft binary:

```bash
qemu-img create -f raw disk.raw 1G
# Write bootloader and Unikraft kernel image to disk.raw
```

### 4.2 Upload Disk Image to Amazon S3

Upload the disk image to your S3 bucket:

```bash
aws s3 cp disk.raw s3://my-unikraft-bucket/disk.raw
```

### 4.3 Import Snapshot

Import the raw image as an EBS snapshot:

```bash
aws ec2 import-snapshot \
  --description "Unikraft ENA Image" \
  --disk-container Format=raw,UserBucket="{S3Bucket=my-unikraft-bucket,S3Key=disk.raw}"
```

Wait until the snapshot status changes to `completed`.

### 4.4 Register the AMI with ENA Support

Register the AMI and enable ENA support with the `--ena-support` flag:

```bash
aws ec2 register-image \
  --name "unikraft-ena-app" \
  --architecture x86_64 \
  --root-device-name "/dev/xvda" \
  --block-device-mappings "[{\"DeviceName\":\"/dev/xvda\",\"Ebs\":{\"SnapshotId\":\"snap-0123456789abcdef0\"}}]" \
  --virtualization-type hvm \
  --ena-support
```

---

## 5. Launch the EC2 Instance

Launch an instance using an ENA-enabled instance type:

```bash
aws ec2 run-instances \
  --image-id ami-0123456789abcdef0 \
  --instance-type t3.nano \
  --key-name my-key-pair \
  --security-group-ids sg-0123456789abcdef0 \
  --subnet-id subnet-0123456789abcdef0
```

Recommended instance types:
- **Testing and Validation**: `t3.nano`, `t3.micro`
- **Compute Workloads**: `c5.large`, `c6i.large`, `c7i.large`
- **Memory Workloads**: `r5.large`, `r6i.large`, `r7i.large`
- **ARM64 Graviton**: `c6g.medium`, `c7g.medium`

---

## 6. Validate Driver and Network Performance

1. Check serial console logs in AWS EC2 Console or via AWS CLI:
   ```bash
   aws ec2 get-console-output --instance-id i-0123456789abcdef0
   ```
2. Verify that the ENA driver initializes and attaches to the network device.
3. Run the benchmark template to create an empty report file and the step-by-step measurement instructions:
   ```bash
   ./scripts/ec2_benchmark.sh
   ```
   The script does not run measurements and does not write any numbers. You run `iperf3` and `netperf` yourself and record the real output.
4. Store the finished report outside version control, for example in the Fossil unversioned store (`fossil uv`). Do not commit reports to this repository.
