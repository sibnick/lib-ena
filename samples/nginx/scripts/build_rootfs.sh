#!/usr/bin/env bash
# Build the initrd CPIO archive for the nginx sample.
#
# Output: initrd.cpio in the sample directory. The Unikraft build embeds
# that file into the unikernel image (CONFIG_LIBVFSCORE_AUTOMOUNT_EINITRD_PATH).
set -euo pipefail

SAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="unikraft-nginx-rootfs"
EXPORT_DIR="${SAMPLE_DIR}/build/rootfs"
CPIO_OUT="${SAMPLE_DIR}/initrd.cpio"

# Directories nginx writes to at run time
RUNTIME_DIRS=(
    run
    tmp
    var/log/nginx
    var/cache/nginx/client_temp
    var/cache/nginx/proxy_temp
    var/cache/nginx/fastcgi_temp
    var/cache/nginx/uwsgi_temp
    var/cache/nginx/scgi_temp
)

echo "[INFO] Building rootfs image from Dockerfile..."
docker build --target rootfs -t "${IMAGE_NAME}" "${SAMPLE_DIR}"

echo "[INFO] Exporting image filesystem..."
rm -rf "${EXPORT_DIR}"
mkdir -p "${EXPORT_DIR}"
# The rootfs image has no entrypoint. Pass a dummy one so the
# container can exist long enough to be exported.
container_id="$(docker create --entrypoint /bin/sh "${IMAGE_NAME}")"
docker export "${container_id}" | tar -x -C "${EXPORT_DIR}"
docker rm "${container_id}" >/dev/null

echo "[INFO] Adding runtime directories and the musl C library name..."
mkdir -p "${EXPORT_DIR}/lib"
for dir in "${RUNTIME_DIRS[@]}"; do
    mkdir -p "${EXPORT_DIR}/${dir}"
done
# musl is one file with two names. nginx asks for libc.musl-x86_64.so.1.
ln -sf ld-musl-x86_64.so.1 "${EXPORT_DIR}/lib/libc.musl-x86_64.so.1"

echo "[INFO] Writing CPIO archive..."
(cd "${EXPORT_DIR}" && find . | cpio -o -H newc -R 0:0 --quiet) > "${CPIO_OUT}"

echo "[SUCCESS] Initrd written to ${CPIO_OUT} ($(du -h "${CPIO_OUT}" | cut -f1))"
