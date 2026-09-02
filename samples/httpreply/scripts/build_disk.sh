#!/usr/bin/env bash
set -euo pipefail

KERNEL_BIN="build/httpreply_qemu-x86_64"
OUTPUT_IMG="build/disk.raw"
STAGING_DIR="build/staging"

echo "[INFO] Creating bootable disk image..."
mkdir -p "${STAGING_DIR}/boot/grub/i386-pc"

# 1. Copy kernel
cp "${KERNEL_BIN}" "${STAGING_DIR}/boot/unikraft.bin"

# 2. Copy GRUB modules
cp /usr/lib/grub/i386-pc/*.mod "${STAGING_DIR}/boot/grub/i386-pc/" || true
cp /usr/lib/grub/i386-pc/*.lst "${STAGING_DIR}/boot/grub/i386-pc/" || true

# 3. Create grub.cfg
cat << 'GRUB_CFG' > "${STAGING_DIR}/boot/grub/grub.cfg"
set default=0
set timeout=0

serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Unikraft HTTP Hello World" {
    multiboot /boot/unikraft.bin
    boot
}
GRUB_CFG

# 4. Create ext4 partition image with mkfs.ext4 -d
rm -f build/part1.img
mkfs.ext4 -F -L "rootfs" -d "${STAGING_DIR}" build/part1.img 63M

# 5. Create raw disk image (64MB)
rm -f "${OUTPUT_IMG}"
dd if=/dev/zero of="${OUTPUT_IMG}" bs=1M count=64 status=none

# 6. Create MBR partition table (part1 starts at 1MiB / 2048 sectors)
/usr/sbin/parted -s "${OUTPUT_IMG}" mklabel msdos
/usr/sbin/parted -s "${OUTPUT_IMG}" mkpart primary ext4 1MiB 100%
/usr/sbin/parted -s "${OUTPUT_IMG}" set 1 boot on

# 7. Write partition data into disk.raw
dd if=build/part1.img of="${OUTPUT_IMG}" bs=1M seek=1 conv=notrunc status=none

# 8. Install GRUB into MBR
cat << 'DEV_MAP' > build/device.map
(hd0) build/disk.raw
DEV_MAP

/usr/sbin/grub-bios-setup -d /usr/lib/grub/i386-pc -m build/device.map "${OUTPUT_IMG}" || {
    echo "[INFO] Using grub-mkimage fallback..."
    /usr/bin/grub-mkimage -O i386-pc -o build/core.img -p "(hd0,msdos1)/boot/grub" biosdisk part_msdos ext2 multiboot normal configfile serial terminfo
    dd if=/usr/lib/grub/i386-pc/boot.img of="${OUTPUT_IMG}" bs=446 count=1 conv=notrunc status=none
    dd if=build/core.img of="${OUTPUT_IMG}" bs=512 seek=1 conv=notrunc status=none
}

echo "[SUCCESS] Disk image created at ${OUTPUT_IMG} (size $(du -h ${OUTPUT_IMG} | cut -f1))"
