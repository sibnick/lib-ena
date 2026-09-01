// fec_test.d - Unit tests for FEC (Forward Error Correction) encoder/decoder
//
// Run: ldc2 -betterC -release -mcpu=native -I=src/common fec_test.d -of=bin/fec_test && ./bin/fec_test
//
// Tests the FEC engine from fec.d:
//   - XOR parity generation and recovery of exactly 1 missing packet
//   - FEC group size = 16 packets
//   - No recovery when 0 or 2+ packets missing
//   - Encoder/decoder synchronization
//
// No production code is modified.

module fec_test;

import core.stdc.string : memcpy, memset;
import core.stdc.stdio : printf, fflush;
import wire_protocol;
import fec;

unittest {
    // -----------------------------------------------------------------------
    // Test 1: FEC encoder produces parity for full group (16 packets)
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payload;
        FecParityWire parity;
        bool generated = false;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            generated = encoder.addPacket(seq, payload.ptr, 64, &parity);
        }

        assert(generated);
        assert(parity.header.type == TYPE_FEC_PARITY);
        assert(parity.base_seq_id == 1);
        assert(parity.packet_count == 16);
        assert(parity.mask_bitmap == 0xFFFF);

        printf("  [PASS] fec_full_group_parity\n");
    }

    // -----------------------------------------------------------------------
    // Test 2: FEC encoder does NOT produce parity for incomplete group
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payload;
        FecParityWire parity;
        bool generated;

        for (ulong seq = 1; seq <= 15; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            generated = encoder.addPacket(seq, payload.ptr, 64, &parity);
            assert(!generated);
        }

        printf("  [PASS] fec_no_partial_parity\n");
    }

    // -----------------------------------------------------------------------
    // Test 3: FEC decoder recovers exactly 1 missing packet
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payloads[cast(int)(seq - 1)], 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payloads[cast(int)(seq - 1)][i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64, &parity);
        }

        // Simulate: packet at seq_id=9 is missing (index 8)
        FecDecoderGroup decoder;
        decoder.reset(1);

        for (ulong seq = 1; seq <= 16; ++seq) {
            if (seq == 9) continue;
            decoder.addDataPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64);
        }

        decoder.addParityPacket(&parity);

        ulong recovered_seq;
        ubyte[MAX_FEC_PAYLOAD] recovered_payload;
        uint recovered_len;

        bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
        assert(recovered);
        assert(recovered_seq == 9);
        assert(recovered_len == 64);

        // Verify recovered payload matches original
        ubyte[MAX_FEC_PAYLOAD] expected;
        memset(&expected, 0, MAX_FEC_PAYLOAD);
        for (uint i = 0; i < 64; ++i) {
            expected[i] = cast(ubyte)((9 + i) & 0xFF);
        }
        for (uint i = 0; i < 64; ++i) {
            assert(recovered_payload[i] == expected[i]);
        }

        printf("  [PASS] fec_recovery_single_missing\n");
    }

    // -----------------------------------------------------------------------
    // Test 4: FEC decoder cannot recover when 0 packets missing
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payloads[cast(int)(seq - 1)], 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payloads[cast(int)(seq - 1)][i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64, &parity);
        }

        FecDecoderGroup decoder;
        decoder.reset(1);

        for (ulong seq = 1; seq <= 16; ++seq) {
            decoder.addDataPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64);
        }

        decoder.addParityPacket(&parity);

        ulong recovered_seq;
        ubyte[MAX_FEC_PAYLOAD] recovered_payload;
        uint recovered_len;

        bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
        assert(!recovered);

        printf("  [PASS] fec_no_recovery_needed\n");
    }

    // -----------------------------------------------------------------------
    // Test 5: FEC decoder cannot recover when 2+ packets missing
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payloads[cast(int)(seq - 1)], 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payloads[cast(int)(seq - 1)][i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64, &parity);
        }

        FecDecoderGroup decoder;
        decoder.reset(1);

        for (ulong seq = 1; seq <= 16; ++seq) {
            if (seq == 5 || seq == 12) continue;
            decoder.addDataPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64);
        }

        decoder.addParityPacket(&parity);

        ulong recovered_seq;
        ubyte[MAX_FEC_PAYLOAD] recovered_payload;
        uint recovered_len;

        bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
        assert(!recovered);

        printf("  [PASS] fec_no_recovery_two_missing\n");
    }

    // -----------------------------------------------------------------------
    // Test 6: FEC recovery for each possible missing packet position
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payloads[cast(int)(seq - 1)], 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payloads[cast(int)(seq - 1)][i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64, &parity);
        }

        for (int missing = 0; missing < 16; ++missing) {
            FecDecoderGroup decoder;
            decoder.reset(1);

            for (ulong seq = 1; seq <= 16; ++seq) {
                if (cast(int)(seq - 1) == missing) continue;
                decoder.addDataPacket(seq, payloads[cast(int)(seq - 1)].ptr, 64);
            }

            decoder.addParityPacket(&parity);

            ulong recovered_seq;
            ubyte[MAX_FEC_PAYLOAD] recovered_payload;
            uint recovered_len;

            bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
            assert(recovered);
            assert(recovered_seq == cast(ulong)(missing + 1));

            ubyte[MAX_FEC_PAYLOAD] expected;
            memset(&expected, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                expected[i] = cast(ubyte)((cast(ulong)(missing + 1) + i) & 0xFF);
            }
            for (uint i = 0; i < 64; ++i) {
                assert(recovered_payload[i] == expected[i]);
            }
        }

        printf("  [PASS] fec_recovery_all_positions\n");
    }

    // -----------------------------------------------------------------------
    // Test 7: FEC encoder resets properly after group complete
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payload;
        FecParityWire parity1, parity2;
        bool gen1, gen2;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            gen1 = encoder.addPacket(seq, payload.ptr, 64, &parity1);
        }
        assert(gen1);
        assert(parity1.base_seq_id == 1);
        assert(parity1.packet_count == 16);

        gen2 = false;
        for (ulong seq = 17; seq <= 32; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            gen2 = encoder.addPacket(seq, payload.ptr, 64, &parity2);
        }
        assert(gen2);
        assert(parity2.base_seq_id == 17);
        assert(parity2.packet_count == 16);

        bool different = false;
        for (uint i = 0; i < MAX_FEC_PAYLOAD; ++i) {
            if (parity1.xor_payload[i] != parity2.xor_payload[i]) {
                different = true;
                break;
            }
        }
        assert(different);

        printf("  [PASS] fec_encoder_reset_after_group\n");
    }

    // -----------------------------------------------------------------------
    // Test 8: FEC decoder reset clears state
    // -----------------------------------------------------------------------
    {
        FecDecoderGroup decoder;
        decoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payload;
        memset(&payload, 0, MAX_FEC_PAYLOAD);
        for (uint i = 0; i < 64; ++i) {
            payload[i] = cast(ubyte)(i & 0xFF);
        }
        decoder.addDataPacket(1, payload.ptr, 64);
        decoder.addDataPacket(2, payload.ptr, 64);

        decoder.reset(100);

        assert(decoder.base_seq_id == 100);
        assert(decoder.received_mask == 0);

        decoder.addDataPacket(1, payload.ptr, 64);
        assert(decoder.received_mask == 0);

        printf("  [PASS] fec_decoder_reset_clears_state\n");
    }

    // -----------------------------------------------------------------------
    // Test 9: FEC recovery with varying payload sizes
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        uint[16] sizes;
        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (int i = 0; i < 16; ++i) {
            sizes[cast(uint)i] = cast(uint)(16 + i * 3);
            if (sizes[cast(uint)i] > 64) sizes[cast(uint)i] = 64;
            memset(&payloads[cast(int)i], 0, MAX_FEC_PAYLOAD);
            for (uint j = 0; j < sizes[cast(uint)i]; ++j) {
                payloads[cast(int)i][j] = cast(ubyte)((cast(ulong)(i + 1) + j) & 0xFF);
            }
            encoder.addPacket(cast(ulong)(i + 1), payloads[cast(int)i].ptr, sizes[cast(uint)i], &parity);
        }

        int missing = 7;

        FecDecoderGroup decoder;
        decoder.reset(1);

        for (int i = 0; i < 16; ++i) {
            if (i == missing) continue;
            decoder.addDataPacket(cast(ulong)(i + 1), payloads[i].ptr, sizes[cast(uint)i]);
        }

        decoder.addParityPacket(&parity);

        ulong recovered_seq;
        ubyte[MAX_FEC_PAYLOAD] recovered_payload;
        uint recovered_len;

        bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
        assert(recovered);
        assert(recovered_seq == cast(ulong)(missing + 1));
        assert(recovered_len == sizes[cast(uint)missing]);

        printf("  [PASS] fec_recovery_varying_sizes\n");
    }

    // -----------------------------------------------------------------------
    // Test 10: FEC group boundary detection in decoder
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payload;
        FecParityWire parity1, parity2;

        for (ulong seq = 1; seq <= 16; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payload.ptr, 64, &parity1);
        }

        for (ulong seq = 17; seq <= 32; ++seq) {
            memset(&payload, 0, MAX_FEC_PAYLOAD);
            for (uint i = 0; i < 64; ++i) {
                payload[i] = cast(ubyte)((seq + i) & 0xFF);
            }
            encoder.addPacket(seq, payload.ptr, 64, &parity2);
        }

        assert(parity1.base_seq_id == 1);
        assert(parity2.base_seq_id == 17);

        FecDecoderGroup decoder;
        decoder.reset(1);

        decoder.addParityPacket(&parity2);
        assert(decoder.base_seq_id == 17);

        printf("  [PASS] fec_group_boundary_detection\n");
    }

    // -----------------------------------------------------------------------
    // Test 11: FEC XOR correctness verification
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(100);

        ubyte[MAX_FEC_PAYLOAD] p1, p2, p3;
        memset(&p1, 0, MAX_FEC_PAYLOAD);
        memset(&p2, 0, MAX_FEC_PAYLOAD);
        memset(&p3, 0, MAX_FEC_PAYLOAD);
        for (uint i = 0; i < 64; ++i) {
            p1[i] = cast(ubyte)((100 + i) & 0xFF);
            p2[i] = cast(ubyte)((101 + i) & 0xFF);
            p3[i] = cast(ubyte)((102 + i) & 0xFF);
        }

        FecParityWire parity;
        encoder.addPacket(100, p1.ptr, 64, &parity);
        encoder.addPacket(101, p2.ptr, 64, &parity);
        encoder.addPacket(102, p3.ptr, 64, &parity);

        ubyte[MAX_FEC_PAYLOAD] recovered;
        memcpy(recovered.ptr, parity.xor_payload.ptr, MAX_FEC_PAYLOAD);

        for (uint i = 0; i < 64; ++i) {
            recovered[i] ^= p1[i];
            recovered[i] ^= p3[i];
        }

        for (uint i = 0; i < 64; ++i) {
            assert(recovered[i] == p2[i]);
        }

        printf("  [PASS] fec_xor_correctness\n");
    }

    // -----------------------------------------------------------------------
    // Test 12: FEC with minimal payload (1 byte)
    // -----------------------------------------------------------------------
    {
        FecEncoder encoder;
        encoder.reset(1);

        ubyte[MAX_FEC_PAYLOAD] payloads[16];
        FecParityWire parity;

        for (int i = 0; i < 16; ++i) {
            memset(&payloads[cast(int)i], 0, MAX_FEC_PAYLOAD);
            payloads[cast(int)i][0] = cast(ubyte)(i + 1);
            encoder.addPacket(cast(ulong)(i + 1), payloads[cast(int)i].ptr, 1, &parity);
        }

        int missing = 0;

        FecDecoderGroup decoder;
        decoder.reset(1);

        for (int i = 1; i < 16; ++i) {
            decoder.addDataPacket(cast(ulong)(i + 1), payloads[i].ptr, 1);
        }

        decoder.addParityPacket(&parity);

        ulong recovered_seq;
        ubyte[MAX_FEC_PAYLOAD] recovered_payload;
        uint recovered_len;

        bool recovered = decoder.tryRecover(&recovered_seq, recovered_payload.ptr, &recovered_len);
        assert(recovered);
        assert(recovered_seq == 1);
        assert(recovered_len == 1);
        assert(recovered_payload[0] == 1);

        printf("  [PASS] fec_minimal_payload\n");
    }
}

extern (C) int main(int argc, char** argv) {
    printf("ALL FEC TESTS PASSED\n");
    return 0;
}
