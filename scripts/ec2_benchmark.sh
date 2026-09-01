#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Authors: Unikraft ENA Driver Maintainers
# Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
#
# EC2 benchmark template for the Unikraft ENA driver.
#
# This script is a TEMPLATE. It contains no measured data and it never
# writes measured numbers. It prints a warning, writes a clearly labeled
# empty report template outside version control, and explains how to run
# the real measurements by hand.
#
# The driver does not expose a statistics API that a script can read.
# Until such an API exists, all numbers in a report must come from an
# external measurement tool (iperf3, netperf) run on real hardware.

set -euo pipefail

# Configuration parameters with defaults
AWS_REGION="${AWS_REGION:-us-east-1}"
INSTANCE_TYPE="${INSTANCE_TYPE:-t3.nano}"
AMI_ID="${AMI_ID:-}"
KEY_NAME="${KEY_NAME:-}"
SUBNET_ID="${SUBNET_ID:-}"
SECURITY_GROUP_ID="${SECURITY_GROUP_ID:-}"

# Reports must never land in version control. Default to an untracked
# directory and refuse to write into a directory that is tracked.
OUTPUT_DIR="${OUTPUT_DIR:-benchmark-results}"
REPORT_MD="${OUTPUT_DIR}/benchmark_report.md"
REPORT_HTML="${OUTPUT_DIR}/benchmark_report.html"

# Validate input parameters to prevent injection
validate_inputs() {
    if [[ ! "${AWS_REGION}" =~ ^[a-z0-9-]+$ ]]; then
        echo "[ERROR] Invalid AWS_REGION format: ${AWS_REGION}" >&2
        exit 1
    fi

    if [[ ! "${INSTANCE_TYPE}" =~ ^[a-zA-Z0-9._-]+$ ]]; then
        echo "[ERROR] Invalid INSTANCE_TYPE format: ${INSTANCE_TYPE}" >&2
        exit 1
    fi

    if [[ ! "${OUTPUT_DIR}" =~ ^[a-zA-Z0-9._/-]+$ ]]; then
        echo "[ERROR] Invalid OUTPUT_DIR format: ${OUTPUT_DIR}" >&2
        exit 1
    fi
}

validate_inputs

if [[ "${OUTPUT_DIR}" == "reports" || "${OUTPUT_DIR}" == "./reports" ]]; then
    echo "[ERROR] REFUSING to write reports into the version-controlled" >&2
    echo "[ERROR] 'reports/' directory. Set OUTPUT_DIR to a path outside" >&2
    echo "[ERROR] version control (default: benchmark-results/)." >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

echo "========================================================"
echo "Unikraft ENA EC2 Benchmark TEMPLATE"
echo "Target Instance : ${INSTANCE_TYPE}"
echo "AWS Region      : ${AWS_REGION}"
echo "Output Directory: ${OUTPUT_DIR}"
echo "========================================================"
echo ""
echo "[WARNING] This script is a template. It does NOT run any" >&2
echo "[WARNING] benchmarks and it does NOT fabricate any numbers." >&2
echo "[WARNING] No measured results exist for this driver yet." >&2
echo ""
echo "To produce a real report, follow these steps by hand:"
echo ""
echo "  1. Build the Unikraft image:"
echo "       kraft build --target kvm --plat qemu --arch x86_64"
echo "  2. Import the image as an ENA-enabled AMI (see docs/ec2_deployment.md)."
echo "  3. Launch two ENA instances of type ${INSTANCE_TYPE} in ${AWS_REGION}."
echo "  4. Install iperf3 and netperf on both instances (a companion"
echo "     measurement image or initramfs is required; none is provided here)."
echo "  5. Run iperf3 (throughput) and netperf (TCP_RR, UDP_RR) between them."
echo "  6. Record the real output of those tools into the report template"
echo "     written below. Only record numbers you actually measured."
echo "  7. Store the finished report outside version control, for example:"
echo "       fossil uv add ${REPORT_MD} --as reports/benchmark_report.md"
echo ""

write_template_reports() {
    local timestamp
    timestamp=$(date -u +"%Y-%m-%d %H:%M:%S UTC")

    cat <<EOF > "${REPORT_MD}"
# Unikraft ENA Benchmark Report (TEMPLATE)

**WARNING: This file is an empty template. It contains NO measured data.**
Do not publish or cite it. Do not add numbers that were not measured.

- **Date created**: ${timestamp}
- **Instance Type (planned)**: ${INSTANCE_TYPE}
- **AWS Region (planned)**: ${AWS_REGION}
- **Driver repository revision**: FILL IN (fossil revision hash)
- **Driver Kconfig**: FILL IN (for example CONFIG_LIBENA=y, CONFIG_LIBENA_LLQ=y)

## 1. Throughput (iperf3)

| Frame Size (bytes) | Queue Mode | Streams | Throughput (Gbps) | Packet Rate (Kpps) |
| :--- | :--- | :--- | :--- | :--- |
| FILL IN | FILL IN | FILL IN | FILL IN | FILL IN |

## 2. Latency (netperf)

| Test | Payload (bytes) | Mode | p50 (us) | p90 (us) | p99 (us) | Trans/sec |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| FILL IN | FILL IN | FILL IN | FILL IN | FILL IN | FILL IN | FILL IN |

## 3. Raw Tool Output

Paste the raw iperf3 / netperf output here. The raw output is the source of
truth for every number in the tables above.

EOF

    cat <<EOF > "${REPORT_HTML}"
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Unikraft ENA Benchmark Report (TEMPLATE)</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; line-height: 1.6; margin: 2rem auto; max-width: 960px; color: #24292e; padding: 0 1rem; }
        h1, h2, h3 { border-bottom: 1px solid #eaecef; padding-bottom: 0.3em; }
        table { border-collapse: collapse; width: 100%; margin: 1.5rem 0; }
        th, td { border: 1px solid #dfe2e5; padding: 8px 12px; text-align: left; }
        th { background-color: #f6f8fa; font-weight: 600; }
        .warning { background: #fff5f5; border: 1px solid #f1b0b0; border-radius: 6px; padding: 1rem; margin-bottom: 1.5rem; }
    </style>
</head>
<body>
    <h1>Unikraft ENA Benchmark Report (TEMPLATE)</h1>
    <div class="warning">
        <strong>WARNING:</strong> This file is an empty template. It contains
        <strong>no measured data</strong>. Do not publish or cite it.
    </div>
    <p>Instance type (planned): ${INSTANCE_TYPE}, region (planned): ${AWS_REGION}, created ${timestamp}.</p>
    <h2>How to fill this report</h2>
    <ol>
        <li>Run the steps printed by scripts/ec2_benchmark.sh.</li>
        <li>Replace every FILL IN field with a real measurement.</li>
        <li>Store the finished report outside version control (fossil uv).</li>
    </ol>
</body>
</html>
EOF

    echo "[INFO] Wrote empty report templates (no measured data):"
    echo "       - Markdown: ${REPORT_MD}"
    echo "       - HTML    : ${REPORT_HTML}"
}

write_template_reports
