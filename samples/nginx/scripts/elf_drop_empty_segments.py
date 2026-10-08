#!/usr/bin/env python3
"""
Drop empty PT_LOAD segments from a Multiboot kernel image.

Unikraft's KVM linker script asks the host linker for one program
header per section group. When a group has no content, the linker still
emits an empty PT_LOAD at the address of the data segment. GRUB reads
the load chunks in address order and rejects two chunks that share an
address: 'error: overlap detected'.

This script turns each empty PT_LOAD into a PT_NULL entry. GRUB skips
PT_NULL entries, and the unikernel does not read the program header
table, so the change is safe.

Usage: elf_drop_empty_segments.py <input-elf> <output-elf>
"""

import struct
import sys

ELF_MAGIC = b"\x7fELF"
PT_LOAD = 1
PT_NULL = 0


def main(argv):
    if len(argv) != 3:
        print("usage: %s <input-elf> <output-elf>" % argv[0], file=sys.stderr)
        return 1

    src, dst = argv[1], argv[2]
    with open(src, "rb") as f:
        data = bytearray(f.read())

    if data[:4] != ELF_MAGIC:
        print("[ERR] %s is not an ELF image" % src, file=sys.stderr)
        return 1
    elf_class = data[4]
    if elf_class not in (1, 2):
        print("[ERR] %s is not a 32-bit or 64-bit ELF image" % src, file=sys.stderr)
        return 1

    if elf_class == 1:
        # ELF32: e_phoff at 0x1c, e_phentsize at 0x2a, e_phnum at 0x2c
        phoff = struct.unpack_from("<I", data, 0x1C)[0]
        phentsize = struct.unpack_from("<H", data, 0x2A)[0]
        phnum = struct.unpack_from("<H", data, 0x2C)[0]
        filesz_at, memsz_at, entry = 16, 20, "<6I"
    else:
        # ELF64: e_phoff at 0x20, e_phentsize at 0x36, e_phnum at 0x38
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize = struct.unpack_from("<H", data, 0x36)[0]
        phnum = struct.unpack_from("<H", data, 0x38)[0]
        filesz_at, memsz_at, entry = 32, 40, "<2I4Q"

    dropped = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        fields = struct.unpack_from(entry, data, off)
        p_type, filesz, memsz = fields[0], fields[filesz_at // 4], fields[memsz_at // 4]
        if elf_class == 2:
            filesz, memsz = fields[4], fields[5]
        if p_type == PT_LOAD and filesz == 0 and memsz == 0:
            struct.pack_into("<I", data, off, PT_NULL)
            dropped += 1
            print("[INFO] segment %d: empty PT_LOAD set to PT_NULL" % i)

    if dropped == 0:
        print("[INFO] no empty PT_LOAD segments found")

    with open(dst, "wb") as f:
        f.write(data)
    print("[SUCCESS] wrote %s (%d segment(s) changed)" % (dst, dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
