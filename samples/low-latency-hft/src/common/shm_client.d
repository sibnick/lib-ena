// Module shm_client.d - POSIX Shared-Memory Ring Client in Dlang betterC
module shm_client;

import core.atomic : atomicFetchAdd, atomicLoad, atomicStore, MemoryOrder;
import core.stdc.stdio : printf, fprintf, stderr;
import core.stdc.stdlib : exit;
import core.stdc.string : memcpy, strerror;
import core.sys.posix.sys.mman : MAP_SHARED, PROT_READ, PROT_WRITE, MAP_FAILED, mmap, munmap;
import core.sys.posix.fcntl : O_CREAT, O_RDWR;

@nogc:
nothrow:

enum uint SHM_MAGIC = 0x53484d31; // "SHM1"
enum uint FRAME_CAP = 520;        // Fits HarnessOrderBook (520 bytes)
enum uint MODE_0600 = 0x180;      // 0600 octal = 384 decimal = 0x180 hex

extern (C) {
    int shm_open(const(char)* name, int oflag, uint mode) @nogc nothrow;
    int shm_unlink(const(char)* name) @nogc nothrow;
    int ftruncate(int fd, long length) @nogc nothrow;
    int close(int fd) @nogc nothrow;
}

align(64) struct ShmSlot {
    shared ulong seq;       // 8 bytes (atomic, offset 0)
    uint frame_len;         // 4 bytes (offset 8)
    ubyte[FRAME_CAP] frame; // 520 bytes (offset 12)
    ubyte[108] _pad;        // Pad to 640 bytes (multiple of 64)
}
static assert(ShmSlot.sizeof == 640);
static assert(ShmSlot.frame_len.offsetof == 8);
static assert(ShmSlot.frame.offsetof == 12);

align(64) struct ShmHeader {
    uint magic;             // 4 bytes
    uint slot_count;        // 4 bytes
    ulong slot_size;        // 8 bytes
    ubyte[48] _pad0;        // Pad to offset 64
    align(64) shared ulong write_index; // 8 bytes (atomic, offset 64)
    ubyte[56] _pad1;        // Pad to 128 bytes
}
static assert(ShmHeader.sizeof == 128);
static assert(ShmHeader.write_index.offsetof == 64);

enum FrameStatus {
    kOk,
    kEmpty,
    kLapped
}

size_t getRegionSize(uint slots) @nogc nothrow {
    return ShmHeader.sizeof + cast(size_t)slots * ShmSlot.sizeof;
}

struct ShmRingClient {
    ShmHeader* header;
    ShmSlot* slots;
    ulong mask;
    void* base;
    size_t region_size;
    int fd;

    bool attach(const(char)* name, uint slots_count, bool create) @nogc nothrow {
        region_size = getRegionSize(slots_count);
        int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
        fd = shm_open(name, flags, MODE_0600);
        if (fd < 0) {
            fprintf(stderr, "shm_open(%s) failed\n", name);
            return false;
        }

        if (create) {
            if (ftruncate(fd, cast(long)region_size) != 0) {
                fprintf(stderr, "ftruncate failed\n");
                close(fd);
                return false;
            }
        }

        base = mmap(null, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED || base is null) {
            fprintf(stderr, "mmap failed\n");
            close(fd);
            return false;
        }

        header = cast(ShmHeader*)base;
        slots = cast(ShmSlot*)(cast(ubyte*)base + ShmHeader.sizeof);

        if (create) {
            header.magic = SHM_MAGIC;
            header.slot_count = slots_count;
            header.slot_size = ShmSlot.sizeof;
            atomicStore!(MemoryOrder.raw)(header.write_index, 0);
            for (uint i = 0; i < slots_count; ++i) {
                atomicStore!(MemoryOrder.raw)(slots[i].seq, 0);
            }
        }

        mask = header.slot_count - 1;
        return true;
    }

    void detach() @nogc nothrow {
        if (base !is null && base != MAP_FAILED) {
            munmap(base, region_size);
            base = null;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    ulong liveEdge() const @nogc nothrow {
        return atomicLoad!(MemoryOrder.seq)(header.write_index);
    }

    void publish(const(void)* frame, uint len) @nogc nothrow {
        ulong idx = atomicLoad!(MemoryOrder.raw)(header.write_index);
        ShmSlot* s = &slots[idx & mask];
        s.frame_len = len;
        memcpy(s.frame.ptr, frame, len);

        atomicStore!(MemoryOrder.rel)(s.seq, idx + 1);
        atomicStore!(MemoryOrder.rel)(header.write_index, idx + 1);
    }

    FrameStatus read(ulong read_index, void* out_buf, uint* out_len, ulong* resume_at) @nogc nothrow {
        ShmSlot* s = &slots[read_index & mask];
        ulong seq = atomicLoad!(MemoryOrder.acq)(s.seq);
        ulong want = read_index + 1;

        if (seq < want) return FrameStatus.kEmpty;
        if (seq > want) {
            ulong edge = liveEdge();
            *resume_at = edge > header.slot_count ? edge - header.slot_count : 0;
            return FrameStatus.kLapped;
        }

        uint len = s.frame_len;
        memcpy(out_buf, s.frame.ptr, len);

        if (atomicLoad!(MemoryOrder.acq)(s.seq) != want) {
            ulong edge = liveEdge();
            *resume_at = edge > header.slot_count ? edge - header.slot_count : 0;
            return FrameStatus.kLapped;
        }

        *out_len = len;
        return FrameStatus.kOk;
    }
}
