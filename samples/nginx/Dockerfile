# Root filesystem for the nginx sample.
#
# Stage 1 takes the official nginx distribution (musl-linked).
# Stage 2 keeps only the files the unikernel needs: the nginx binary,
# its dynamic loader and shared libraries, and the web content.
#
# scripts/build_rootfs.sh exports this image into a CPIO initrd archive.

FROM nginx:1.27-alpine AS nginx

FROM scratch AS rootfs

# Dynamic loader and C library (musl). The loader is also the C library:
# build_rootfs.sh adds the libc.musl-x86_64.so.1 name that nginx asks for.
COPY --from=nginx /lib/ld-musl-x86_64.so.1 /lib/ld-musl-x86_64.so.1

# Shared libraries listed in the nginx ELF header
COPY --from=nginx /usr/lib/libpcre2-8.so.0 /usr/lib/libpcre2-8.so.0
COPY --from=nginx /usr/lib/libssl.so.3 /usr/lib/libssl.so.3
COPY --from=nginx /usr/lib/libcrypto.so.3 /usr/lib/libcrypto.so.3
COPY --from=nginx /usr/lib/libz.so.1 /usr/lib/libz.so.1

# nginx executable
COPY --from=nginx /usr/sbin/nginx /usr/sbin/nginx

# Configuration for single-process operation on Unikraft
COPY conf/nginx.conf /etc/nginx/nginx.conf
COPY conf/mime.types /etc/nginx/mime.types

# User and group database. musl reads these files for user lookups.
COPY --from=nginx /etc/passwd /etc/passwd
COPY --from=nginx /etc/group /etc/group

# Web content
COPY html/index.html /usr/share/nginx/html/index.html
