#!/usr/bin/env bash
# Build a GRUB-bootable raw disk image for the nginx sample.
#
# The disk carries GRUB, the unikernel, and the CPIO initrd. GRUB loads
# the initrd as a multiboot module, and the unikernel unpacks it into
# ramfs at / (CONFIG_LIBVFSCORE_AUTOMOUNT_CI_INITRD).
# Run scripts/build_rootfs.sh and the unikernel build before this script.
#
# Optional environment variables:
#   KERNEL_BIN   - path to the unikernel binary (default: build/*_qemu-x86_64)
#   NETDEV_IP    - static IPv4 for the ENA interface, e.g. 172.31.16.153/20
#   NETDEV_GW    - gateway for the static IPv4, e.g. 172.31.16.1
#   OUTPUT_IMG   - path of the raw disk image (default: build/disk.raw)
set -euo pipefail

SAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${SAMPLE_DIR}/build"
OUTPUT_IMG="${OUTPUT_IMG:-${BUILD_DIR}/disk.raw}"
STAGING_DIR="${BUILD_DIR}/staging"

if [ $# -ge 1 ]; then
    KERNEL_BIN="$1"
else
    # kraft build writes to .unikraft/build/, the make flow writes to build/.
    KERNEL_BIN="$(find "${BUILD_DIR}" "${SAMPLE_DIR}/.unikraft/build" -maxdepth 1 \
        -name '*_qemu-x86_64' ! -name '*.dbg' ! -name '*.cmd' 2>/dev/null | sort | head -1)"
fi

if [ -z "${KERNEL_BIN:-}" ] || [ ! -f "${KERNEL_BIN}" ]; then
    echo "[ERR] Unikernel binary not found. Build it first:"
    echo "[ERR]   kraft build --target aws-t3-x86_64"
    exit 1
fi

if [ ! -f "${SAMPLE_DIR}/initrd.cpio" ]; then
    echo "[ERR] ${SAMPLE_DIR}/initrd.cpio is missing."
    echo "[ERR] The disk needs the nginx rootfs. Run:"
    echo "[ERR]   ./scripts/build_rootfs.sh"
    exit 1
fi

# Kernel command line. Without NETDEV_IP the stack uses DHCP.
# ukboot parses library parameters only up to ' --'. Everything after
# that goes to the application. Without the separator nginx receives
# netdev.ip=... and exits with 'invalid option'. Only add the
# separator when a library parameter is present: a trailing ' --'
# with no library parameter before it reaches nginx as a stray argument.
KERNEL_ARGS="unikraft"
if [ -n "${NETDEV_IP:-}" ]; then
    KERNEL_ARGS="${KERNEL_ARGS} netdev.ip=${NETDEV_IP}"
    if [ -n "${NETDEV_GW:-}" ]; then
        KERNEL_ARGS="${KERNEL_ARGS}:${NETDEV_GW}"
    fi
    KERNEL_ARGS="${KERNEL_ARGS} --"
fi

echo "[INFO] Creating bootable disk image..."
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/boot/grub/i386-pc"

# 1. Copy kernel and initrd.
# The host linker can leave an empty PT_LOAD in the kernel image. GRUB
# rejects that image ('overlap detected'), so drop the empty segment.
python3 "${SAMPLE_DIR}/scripts/elf_drop_empty_segments.py" \
    "${KERNEL_BIN}" "${STAGING_DIR}/boot/unikraft.bin"
cp "${SAMPLE_DIR}/initrd.cpio" "${STAGING_DIR}/boot/initrd.cpio"

# 2. Copy GRUB modules
cp /usr/lib/grub/i386-pc/*.mod "${STAGING_DIR}/boot/grub/i386-pc/" || true
cp /usr/lib/grub/i386-pc/*.lst "${STAGING_DIR}/boot/grub/i386-pc/" || true

# 3. Create grub.cfg
cat > "${STAGING_DIR}/boot/grub/grub.cfg" << GRUB_CFG
set default=0
set timeout=0

serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Unikraft nginx (AWS ENA)" {
    multiboot /boot/unikraft.bin ${KERNEL_ARGS}
    module /boot/initrd.cpio
    boot
}
GRUB_CFG

# 4. Create ext4 partition image with mkfs.ext4 -d
rm -f "${BUILD_DIR}/part1.img"
mkfs.ext4 -F -L "rootfs" -d "${STAGING_DIR}" "${BUILD_DIR}/part1.img" 127M

# 5. Create raw disk image (128MB)
rm -f "${OUTPUT_IMG}"
dd if=/dev/zero of="${OUTPUT_IMG}" bs=1M count=128 status=none

# 6. Create MBR partition table (part1 starts at 1MiB / 2048 sectors)
/usr/sbin/parted -s "${OUTPUT_IMG}" mklabel msdos
/usr/sbin/parted -s "${OUTPUT_IMG}" mkpart primary ext4 1MiB 100%
/usr/sbin/parted -s "${OUTPUT_IMG}" set 1 boot on

# 7. Write partition data into the raw disk
dd if="${BUILD_DIR}/part1.img" of="${OUTPUT_IMG}" bs=1M seek=1 conv=notrunc status=none

# 8. Install GRUB into MBR
cat << DEV_MAP > "${BUILD_DIR}/device.map"
(hd0) ${OUTPUT_IMG}
DEV_MAP

/usr/sbin/grub-bios-setup -d /usr/lib/grub/i386-pc -m "${BUILD_DIR}/device.map" "${OUTPUT_IMG}" || {
    echo "[INFO] Using grub-mkimage fallback..."
    /usr/bin/grub-mkimage -O i386-pc -o "${BUILD_DIR}/core.img" -p "(hd0,msdos1)/boot/grub" biosdisk part_msdos ext2 multiboot normal configfile serial terminfo
    dd if=/usr/lib/grub/i386-pc/boot.img of="${OUTPUT_IMG}" bs=446 count=1 conv=notrunc status=none
    dd if="${BUILD_DIR}/core.img" of="${OUTPUT_IMG}" bs=512 seek=1 conv=notrunc status=none
}

echo "[SUCCESS] Disk image created at ${OUTPUT_IMG} (size $(du -h "${OUTPUT_IMG}" | cut -f1))"
