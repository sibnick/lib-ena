// Module fec.d - Forward Error Correction (FEC) Engine for Low-Latency Transport
module fec;

import core.stdc.string : memcpy, memset;
import wire_protocol;

@nogc:
nothrow:

enum uint FEC_GROUP_SIZE = 16;
enum uint MAX_FEC_PAYLOAD = 256;

// XOR vector accumulation helper (LLVM auto-vectorizes to 256-bit unaligned AVX2 vxorps/vmovups SIMD)
void xorBuffers(ubyte* dst, const(ubyte)* src, uint len) @nogc nothrow {
    ulong* d = cast(ulong*)dst;
    const(ulong)* s = cast(const(ulong)*)src;
    uint u64_count = len / ulong.sizeof;

    for (uint i = 0; i < u64_count; ++i) {
        d[i] ^= s[i];
    }

    uint rem_start = u64_count * cast(uint)ulong.sizeof;
    for (uint i = rem_start; i < len; ++i) {
        dst[i] ^= src[i];
    }
}

struct FecEncoder {
    ulong group_id = 1;
    ulong base_seq_id = 0;
    ushort packet_count = 0;
    ushort mask_bitmap = 0;
    ubyte[MAX_FEC_PAYLOAD] parity_buf;

    void reset(ulong current_seq) @nogc nothrow {
        base_seq_id = current_seq;
        packet_count = 0;
        mask_bitmap = 0;
        memset(parity_buf.ptr, 0, MAX_FEC_PAYLOAD);
    }

    // Process a newly sent data packet; returns true if FEC group is complete and parity packet is ready
    bool addPacket(ulong seq_id, const(ubyte)* payload, uint len, FecParityWire* out_parity) @nogc nothrow {
        if (packet_count == 0) {
            base_seq_id = seq_id;
        }

        uint index = cast(uint)(seq_id - base_seq_id);
        if (index < FEC_GROUP_SIZE) {
            mask_bitmap |= cast(ushort)(1 << index);
            uint copy_len = len > MAX_FEC_PAYLOAD ? MAX_FEC_PAYLOAD : len;
            xorBuffers(parity_buf.ptr, payload, copy_len);
            packet_count++;
        }

        if (packet_count >= FEC_GROUP_SIZE) {
            out_parity.header.seq_id = seq_id;
            out_parity.header.send_ts_ns = 0; // Filled by caller
            out_parity.header.type = TYPE_FEC_PARITY;
            out_parity.header.version_ = 1;

            out_parity.group_id = group_id++;
            out_parity.base_seq_id = base_seq_id;
            out_parity.packet_count = packet_count;
            out_parity.mask_bitmap = mask_bitmap;
            memcpy(out_parity.xor_payload.ptr, parity_buf.ptr, MAX_FEC_PAYLOAD);

            reset(seq_id + 1);
            return true;
        }

        return false;
    }
}

struct FecDecoderGroup {
    ulong group_id;
    ulong base_seq_id;
    ushort expected_count;
    ushort received_mask;
    ubyte[MAX_FEC_PAYLOAD][FEC_GROUP_SIZE] packet_bufs;
    uint[FEC_GROUP_SIZE] packet_lens;
    bool has_parity;
    FecParityWire parity;

    void reset(ulong base_seq) @nogc nothrow {
        base_seq_id = base_seq;
        expected_count = FEC_GROUP_SIZE;
        received_mask = 0;
        has_parity = false;
        for (uint i = 0; i < FEC_GROUP_SIZE; ++i) {
            packet_lens[i] = 0;
        }
    }

    void addDataPacket(ulong seq_id, const(ubyte)* payload, uint len) @nogc nothrow {
        if (base_seq_id == 0) base_seq_id = seq_id;
        if (seq_id < base_seq_id) return;

        uint idx = cast(uint)(seq_id - base_seq_id);
        if (idx < FEC_GROUP_SIZE) {
            received_mask |= cast(ushort)(1 << idx);
            uint copy_len = len > MAX_FEC_PAYLOAD ? MAX_FEC_PAYLOAD : len;
            memcpy(packet_bufs[idx].ptr, payload, copy_len);
            packet_lens[idx] = copy_len;
        }
    }

    void addParityPacket(const FecParityWire* parity_wire) @nogc nothrow {
        parity = *parity_wire;
        has_parity = true;
        base_seq_id = parity_wire.base_seq_id;
        expected_count = parity_wire.packet_count;
    }

    // Try reconstructing missing packet if exactly 1 packet in group is missing
    bool tryRecover(ulong* out_recovered_seq, ubyte* out_payload, uint* out_len) @nogc nothrow {
        if (!has_parity) return false;

        ushort missing_mask = cast(ushort)(parity.mask_bitmap & ~received_mask);
        // Count missing bits
        uint missing_count = 0;
        uint missing_index = 0;
        for (uint i = 0; i < parity.packet_count; ++i) {
            if ((missing_mask & (1 << i)) != 0) {
                missing_count++;
                missing_index = i;
            }
        }

        // Recovery is possible if exactly 1 packet is missing
        if (missing_count == 1) {
            ubyte[MAX_FEC_PAYLOAD] rec;
            memcpy(rec.ptr, parity.xor_payload.ptr, MAX_FEC_PAYLOAD);

            uint max_len = 0;
            for (uint i = 0; i < parity.packet_count; ++i) {
                if (i != missing_index && (received_mask & (1 << i)) != 0) {
                    xorBuffers(rec.ptr, packet_bufs[i].ptr, packet_lens[i]);
                    if (packet_lens[i] > max_len) max_len = packet_lens[i];
                }
            }

            *out_recovered_seq = base_seq_id + missing_index;
            memcpy(out_payload, rec.ptr, max_len);
            *out_len = max_len;

            // Mark as recovered
            received_mask |= cast(ushort)(1 << missing_index);
            return true;
        }

        return false;
    }
}
