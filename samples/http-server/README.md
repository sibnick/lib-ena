# Unikraft Hello-HTTP on AWS (native ENA)

A minimal `Hello, World!` HTTP server (port 80) that runs as a Unikraft
unikernel on a real AWS EC2 instance (`t3.nano`). Networking uses the native
AWS ENA driver (`lib-ena`) on top of lwIP (DHCP/TCP/sockets) and nolibc.

The unikernel is packed as a multiboot ELF image, GRUB boots it from an
EBS-backed AMI, and the result is a web server:

```
$ curl -i http://<instance-ip>/
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 13
Connection: close
Server: Unikraft-ENA/1.0

Hello, World!
```

## Directory layout

| Path | Purpose |
|------|---------|
| `main.c` | Web server entry point using lwIP sockets API |
| `Config.uk` | Kconfig dependencies (nolibc, lwIP, ENA, netdev) |
| `Makefile` | Build entry point; applies patches and calls Unikraft build system |
| `Kraftfile` | Application definition for the `kraft` CLI (references parent `lib-ena`) |
| `patches/` | Linker script fixes for KVM x86 multiboot loading |
| `scripts/apply_patches.sh` | Idempotent patch applier |
| `scripts/build_disk.sh` | Builds raw bootable disk image with GRUB |
| `scripts/deploy_aws.py` | End-to-end AWS deployment (build → disk image → EBS snapshot → AMI → EC2) |

## Build

```sh
git submodule update --init --recursive
make
```

The `Makefile` first runs `scripts/apply_patches.sh`, then builds through the
Unikraft build system. Output:

- `build/unikraft-test_qemu-x86_64` — the ELF unikernel with an embedded
  multiboot (v1) header; this is what GRUB loads.
- `build/unikraft-test_qemu-x86_64.bootinfo` — UKBI blob (BIOS/SeaBIOS flow).

Note: the `kraft` CLI build flow does not run the patch step. If you build
with `kraft build`, run `./scripts/apply_patches.sh` first.

## Deploy to AWS

```sh
SUBNET_ID=subnet-... PRIVATE_IP=172.31.x.y \
GATEWAY_IP=172.31.x.1   NETMASK=255.255.240.0 \
python3 scripts/deploy_aws.py
```

Required environment: `SUBNET_ID`, `PRIVATE_IP`, `GATEWAY_IP`, `NETMASK`
(the latter three are baked into the GRUB netdev/kernel command line of the
bootable image). Optional: `AWS_REGION` (default `us-east-1`),
`INSTANCE_TYPE` (default `t3.nano`).

The script then: builds the unikernel, assembles a bootable disk image with
GRUB, uploads it to a new EBS snapshot via the EBS Direct API, registers an
AMI from it, launches the instance, and prints the public IP to test with
`curl`.

## Patches

Upstream submodule repositories are kept pristine: no local commits land in
them. Instead, the pinned submodule revisions are delta-patched from this
repository:

- **Convention:** `patches/<submodule-name>-<base-sha8>.patch` — the suffix
  is the 8-character commit the patch was generated against, e.g.
  `unikraft-e31b2c44.patch` applies to `.unikraft/unikraft` at `e31b2c44`.
- **Applying:** `scripts/apply_patches.sh` applies every patch in `patches/`.
  It is idempotent (already-applied patches are detected and skipped), warns
  if the submodule is at an unexpected commit, and aborts with instructions
  if a patch   cannot be applied. The `Makefile` invokes it automatically
  before every build goal. After a build, the patched submodules show
  local modifications in their own `git status` — that is expected; the
  deltas live in this repository's `patches/` and are never committed to
  the upstream repos.
- **Regenerating:** after modifying a submodule locally,
  `git -C <submodule> diff > patches/<name>-<sha8>.patch`.

### `unikraft-e31b2c44.patch` — required: drop TLS pheaders from the KVM x86 linker script

```diff
--- a/plat/kvm/x86/link64.lds.S
+++ b/plat/kvm/x86/link64.lds.S
@@ -37,8 +37,6 @@ PHDRS
  	text PT_LOAD FLAGS(PHDRS_PF_RX);
  	rodata PT_LOAD FLAGS(PHDRS_PF_R);
  	data PT_LOAD;
-	tls PT_TLS;
-	tls_load PT_LOAD;
  	stack PT_GNU_STACK FLAGS(PHDRS_PF_RW);
  }
```

**Why it is needed.** The standard KVM x86 linker script declares the
classic dual-header TLS layout: a `tls` PT_TLS segment (TLS size metadata)
plus a `tls_load` PT_LOAD segment (the initial-copy the multiboot loader
actually loads). This breaks at the GRUB level for this particular image:

1. The project's dependencies use C11 `__thread` storage — `libnolibc`
   (`errno`) and `libposix_user` (passwd/group) — so the image carries TLS
   sections. It has `.tbss` (24 bytes) but **no** `.tdata` (there are no
   initialized TLS variables, so nothing has file-backed initial data).
2. With `.tdata` empty, the linker page-aligns `.tbss` onto the same page as
   `.data` (`0x00187000`). The resulting ELF therefore contains two
   **overlapping PT_LOAD pheaders** with the same `p_paddr=0x00187000`:
   `data` (memsz `0x47000`) and an *empty* `tls_load` (memsz `0x0`).
3. GRUB's multiboot (v1) loader allocates a relocator chunk at the exact
   physical address for every PT_LOAD segment
   (`grub-core/loader/multiboot_elfxx.c`), and
   `grub_relocator_alloc_chunk_addr`
   (`grub-core/lib/relocator.c:1246`) rejects overlapping ranges:

   ```
   error: overlap detected.
   error: you need to load the kernel first.
   Failed to boot both default and fallback entries.
   ```

   The kernel is never loaded, so the unikernel never even starts.

**Why it is safe.** Unikraft's own TLS implementation
(`arch/x86/x86_64/tls.c`) does not read ELF program headers at boot time
(it notes they are unavailable when booting as a guest): it sizes each
thread's TLS area from the `_tls_start`/`_etdata`/`_tls_end` symbols,
allocates the area from the heap, copies the `.tdata` master template (zero
bytes in this image) and zeroes `.tbss`. Those symbol values are identical
with and without the patch, so removing the pheader declarations changes
nothing at runtime — it only removes the two overlapping pheaders that GRUB
refuses to load. The image then contains exactly three non-overlapping
PT_LOAD segments (`text`, `rodata`, `data`), with `.tbss` folded into the
read-write data segment.

Verified in both directions: the unpatched image fails in GRUB with
`overlap detected`; the patched image boots on EC2 and serves HTTP.

### Note on the (former) lib-lwip patch

An earlier revision of this project also carried a patch to
`.libs/lib-lwip` (forcing a background RX poll thread in `uknetdev.c` and
wiring `tcpip_input`). During development it turned out to be unnecessary:
unmodified `RELEASE-0.21.0` (`ec55ae17`) already starts the `_poll_netif`
thread via `uknetdev_updown()` when the autoiface brings the interface up
(ENA advertises no interrupt RXQ feature, so the poll path is taken), and
`uknetdev_addif()` already passes `tcpip_input` to `netif_add()`. The patch
was removed and the `lib-lwip` submodule is unmodified. The applier script
still understands the `lib-lwip-*` naming convention for future patches.
