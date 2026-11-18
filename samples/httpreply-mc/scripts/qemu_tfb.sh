#!/usr/bin/env bash
#
# Local 1-vCPU KVM run of httpreply-mc on virtio-net for the TFB
# over-the-wire check. QEMU has no ENA device, so this build (see
# configs/qemu-1q.seed) drops nothing at run time except the ENA
# driver: the app serves the same tfb_http.h endpoints on port 8080
# through user-mode networking. [Ticket 2212657476]
#
# Build the image first. See the header of configs/qemu-1q.seed:
# start from the full expanded .config, set CONFIG_UKPLAT_CPU_MAXCOUNT=1
# and CONFIG_LIBUKNETDEV_MAXNBQUEUES=1, then:
#   make all
#   scripts/build_disk.sh
#
# Usage: ./scripts/qemu_tfb.sh [run_seconds] [host_port]
# In another shell:
#   CHECK_LEGACY=1 scripts/tfb_validate.sh 127.0.0.1 8080
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-120}"
HOST_PORT="${2:-8080}"
DISK="build/disk.raw"
LOG="qemu_tfb.log"

if [ ! -f "$DISK" ]; then
	echo "[ERR] Disk image not found: $DISK"
	echo "[ERR] Build the qemu variant and run scripts/build_disk.sh first."
	exit 1
fi

rm -f "$LOG"

echo "[INFO] Starting QEMU for ${DUR}s. Host port ${HOST_PORT} -> guest 8080."
echo "[INFO] Console log: $LOG"

timeout "$DUR" qemu-system-x86_64 \
	-machine q35 \
	-enable-kvm \
	-cpu host \
	-smp 1 \
	-m 1G \
	-display none \
	-no-reboot \
	-serial file:"$LOG" \
	-drive file="$DISK",format=raw,if=ide,index=0 \
	-netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:8080 \
	-device virtio-net-pci,netdev=net0 \
	|| true

echo "[done] QEMU run finished. Log: $LOG"