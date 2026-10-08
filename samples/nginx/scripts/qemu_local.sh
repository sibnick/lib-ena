#!/usr/bin/env bash
# Run the nginx sample in QEMU with user-mode networking.
#
# QEMU has no ENA device, so this run uses virtio-net. It checks the
# rootfs, the ELF loader, and nginx. It does not check the ENA driver.
#
# Usage: ./scripts/qemu_local.sh [run_seconds] [host_port]
# Then in another shell: ./scripts/smoke_test.sh 127.0.0.1:8080
set -euo pipefail

SAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DURATION="${1:-60}"
HOST_PORT="${2:-8080}"
DISK="${SAMPLE_DIR}/build/disk.raw"
LOG="${SAMPLE_DIR}/build/qemu_local.log"

if [ ! -f "${DISK}" ]; then
    echo "[ERR] Disk image not found: ${DISK}"
    echo "[ERR] Run ./scripts/build_disk.sh first."
    exit 1
fi

echo "[INFO] Starting QEMU for ${DURATION}s. Host port ${HOST_PORT} forwards to guest port 80."
echo "[INFO] Console log: ${LOG}"

timeout "${DURATION}" qemu-system-x86_64 \
    -machine q35 \
    -enable-kvm \
    -cpu host \
    -smp 1 \
    -m 512M \
    -display none \
    -no-reboot \
    -serial file:"${LOG}" \
    -drive file="${DISK}",format=raw,if=ide,index=0 \
    -netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:80 \
    -device virtio-net-pci,netdev=net0 \
    || true

echo "[done] QEMU run finished. Console log: ${LOG}"
