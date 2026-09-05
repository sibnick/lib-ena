---
name: ec2-perf-testing
description: Instructions and procedures for deploying, executing, measuring, and reporting HTTP performance benchmarks for Unikraft ENA against Linux baselines on AWS EC2 hardware.
---

# EC2 Performance Testing and Reporting Skill for AI Agents

This skill provides step-by-step instructions for AI agents to run HTTP benchmarks on AWS EC2. It covers image preparation, instance deployment, benchmark execution, metric collection, reporting, and resource cleanup.

---

## 1. Overview

Performance benchmarks compare the Unikraft native ENA driver and lwIP stack against a standard Linux distribution (such as Ubuntu 24.04).

Key testing rules:
- Run benchmarks on compute-optimized instances with dedicated CPU cores (such as `c6i.large`). Do not use burstable instances (such as `t3.nano`) because CPU credit throttling skews results.
- Deploy the client instance and target instances into the same availability zone and subnet to minimize network variance.
- Run `wrk` with keep-alive connections across multiple concurrency levels.
- Store measured results outside version control in the Fossil unversioned store (`fossil uv`).
- Always terminate all cloud resources immediately after test completion.

---

## 2. Infrastructure Setup Workflow

### A. Environment Parameters

Define the target environment variables:

```bash
export AWS_REGION="us-east-1"
export INSTANCE_TYPE="c6i.large"
export SUBNET_ID="<subnet-id>"
export SG_ID="<security-group-id>"
export KEY_NAME="<key-name>"
```

### B. Build and Register the Unikraft ENA Image

1. Build the KVM image using KraftKit:
   ```bash
   cd samples/httpreply
   kraft build --target aws-t3-x86_64
   ```

2. Create a raw disk image and copy the binary:
   ```bash
   dd if=/dev/zero of=disk.raw bs=1M count=64
   mkfs.ext4 -F disk.raw
   mkdir -p mnt
   sudo mount disk.raw mnt
   sudo cp build/httpreply_qemu-x86_64 mnt/kernel
   sudo umount mnt
   ```

3. Upload the snapshot with EBS Direct APIs or AWS CLI, and register an ENA-enabled AMI:
   ```bash
   aws ec2 register-image \
     --region "${AWS_REGION}" \
     --name "unikraft-httpreply-$(date +%s)" \
     --architecture x86_64 \
     --root-device-name "/dev/xvda" \
     --ena-support \
     --block-device-mappings "[{\"DeviceName\":\"/dev/xvda\",\"Ebs\":{\"SnapshotId\":\"${SNAPSHOT_ID}\",\"VolumeType\":\"gp3\"}}]"
   ```

### C. Launch Test Instances

1. Launch the Unikraft target instance:
   ```bash
   aws ec2 run-instances \
     --region "${AWS_REGION}" \
     --image-id "${UNIKRAFT_AMI_ID}" \
     --instance-type "${INSTANCE_TYPE}" \
     --key-name "${KEY_NAME}" \
     --subnet-id "${SUBNET_ID}" \
     --security-group-ids "${SG_ID}" \
     --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=unikraft-perf-target}]'
   ```

2. Launch the companion Linux baseline instance (Ubuntu 24.04):
   ```bash
   aws ec2 run-instances \
     --region "${AWS_REGION}" \
     --image-id "${UBUNTU_AMI_ID}" \
     --instance-type "${INSTANCE_TYPE}" \
     --key-name "${KEY_NAME}" \
     --subnet-id "${SUBNET_ID}" \
     --security-group-ids "${SG_ID}" \
     --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=linux-perf-target}]'
   ```

3. Launch the client benchmark runner instance in the same subnet.

---

## 3. Benchmark Execution Procedure

Run benchmarks from the client instance using `wrk`.

### A. Test Execution Matrix

Sweep concurrency levels: `1`, `5`, `10`, `25`, `50`, `100`, `200`.

Run `wrk` with 2 threads and a duration of 10 to 30 seconds per step:

```bash
wrk -t2 -c<concurrency> -d10s --latency http://<target-ip>/
```

### B. Output Extraction

Extract the following metrics from each test run:
- Requests per second (`Requests/sec`)
- Average latency and standard deviation
- Maximum latency (`Latency ... Max`)
- Latency percentiles (50%, 75%, 90%, 99%)
- Network throughput (`Transfer/sec`)
- Socket error counters (`connect`, `read`, `write`, `timeout`)

---

## 4. Reporting and Data Archiving

### A. Generate Machine-Readable Reports

Record output in both CSV and JSON formats:

```csv
concurrency,target,requests_sec,latency_avg_ms,latency_stdev_ms,latency_max_ms,transfer_kb_sec,total_requests,socket_errors
1,Unikraft (Optimized ENA + lwIP),4190.53,0.24,0.01,0.39,613.85,41906,0
1,Linux (Ubuntu 24.04 ENA),6296.08,0.16,0.01,0.29,921.60,63592,0
```

### B. Store Reports in Fossil Unversioned Storage

Store all raw test outputs and generated CSV/JSON files in `fossil uv`:

```bash
fossil uv add benchmark_results.csv --as reports/httpreply_c6i_benchmark_results.csv
fossil uv add benchmark_results.json --as reports/httpreply_c6i_benchmark_results.json
fossil uv sync
```

### C. Update Documentation

Update `samples/httpreply/README.md` and repository documentation.
Follow ASD-STE100 rules:
- Write active sentences.
- Keep instruction sentences under 20 words.
- Do not use semicolons.
- Use simple terms (`start`, `use`, `show`).
- Do not include marketing adjectives (`seamless`, `blazing-fast`).

---

## 5. Teardown and Cleanup Checklist

Always verify and clean up resources after the benchmark finishes:

1. **Terminate EC2 Instances**:
   ```bash
   aws ec2 terminate-instances --region "${AWS_REGION}" --instance-ids <id1> <id2> <id3>
   ```

2. **Deregister AMIs**:
   ```bash
   aws ec2 deregister-image --region "${AWS_REGION}" --image-id <ami-id>
   ```

3. **Delete EBS Snapshots**:
   ```bash
   aws ec2 delete-snapshot --region "${AWS_REGION}" --snapshot-id <snapshot-id>
   ```

4. **Delete Security Groups and Key Pairs**:
   ```bash
   aws ec2 delete-security-group --region "${AWS_REGION}" --group-id <sg-id>
   aws ec2 delete-key-pair --region "${AWS_REGION}" --key-name <key-name>
   ```

5. **Verify Clean State**:
   Make sure no instances or orphaned EBS volumes remain:
   ```bash
   aws ec2 describe-instances --region "${AWS_REGION}" --filters "Name=instance-state-name,Values=pending,running,stopping,stopped"
   aws ec2 describe-volumes --region "${AWS_REGION}" --filters "Name=status,Values=available,in-use"
   ```
