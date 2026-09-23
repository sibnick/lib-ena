#!/usr/bin/env bash
#
# Local 2-vCPU KVM run of the httpreply-mc image for verification.
# No ENA device exists in QEMU, so the guest has no netif. The
# test target is boot, per-core allocator init, and concurrent
# worker spin without allocator corruption.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-100}"

rm -f qemu_run.log

timeout "$DUR" qemu-system-x86_64 \
	-enable-kvm \
	-cpu host \
	-smp 2 \
	-m 2G \
	-nographic \
	-no-reboot \
	-serial file:qemu_run.log \
	-drive file=build/disk.raw,format=raw,if=ide,index=0 \
	|| true

echo "[done] QEMU run finished (timeout or halt). Log: qemu_run.log"
