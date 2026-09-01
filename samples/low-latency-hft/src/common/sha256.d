// Module sha256.d - SHA-256 Checksum Verification Engine for betterC mode
module sha256;

import core.stdc.stdio : sprintf;

@nogc:
nothrow:

private uint rotr(uint x, uint n) @nogc nothrow {
    return (x >> n) | (x << (32 - n));
}

private uint choose(uint e, uint f, uint g) @nogc nothrow {
    return (e & f) ^ (~e & g);
}

private uint majority(uint a, uint b, uint c) @nogc nothrow {
    return (a & b) ^ (a & c) ^ (b & c);
}

private uint sig0(uint x) @nogc nothrow {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

private uint sig1(uint x) @nogc nothrow {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

private uint gamma0(uint x) @nogc nothrow {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

private uint gamma1(uint x) @nogc nothrow {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

private immutable uint[64] K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
];

struct Sha256 {
    uint[8] state = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ];
    ubyte[64] buffer;
    uint buf_len = 0;
    ulong bit_count = 0;

    void update(const(ubyte)* data, size_t len) @nogc nothrow {
        for (size_t i = 0; i < len; ++i) {
            buffer[buf_len++] = data[i];
            if (buf_len == 64) {
                transform(buffer.ptr);
                buf_len = 0;
            }
            bit_count += 8;
        }
    }

    private void transform(const(ubyte)* chunk) @nogc nothrow {
        uint[64] w;
        for (uint i = 0; i < 16; ++i) {
            w[i] = (cast(uint)chunk[i * 4] << 24) |
                   (cast(uint)chunk[i * 4 + 1] << 16) |
                   (cast(uint)chunk[i * 4 + 2] << 8) |
                   (cast(uint)chunk[i * 4 + 3]);
        }
        for (uint i = 16; i < 64; ++i) {
            w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
        }

        uint a = state[0], b = state[1], c = state[2], d = state[3];
        uint e = state[4], f = state[5], g = state[6], h = state[7];

        for (uint i = 0; i < 64; ++i) {
            uint temp1 = h + sig1(e) + choose(e, f, g) + K[i] + w[i];
            uint temp2 = sig0(a) + majority(a, b, c);
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void finalHex(char[65]* out_hex) @nogc nothrow {
        ubyte[64] pad;
        size_t pad_len = (buf_len < 56) ? (56 - buf_len) : (120 - buf_len);
        pad[0] = 0x80;
        for (size_t i = 1; i < pad_len; ++i) pad[i] = 0;
        update(pad.ptr, pad_len);

        ubyte[8] bits_bytes;
        for (int i = 7; i >= 0; --i) {
            bits_bytes[7 - i] = cast(ubyte)(bit_count >> (i * 8));
        }
        update(bits_bytes.ptr, 8);

        for (int i = 0; i < 8; ++i) {
            sprintf(out_hex.ptr + i * 8, "%08x", state[i]);
        }
        (*out_hex)[64] = '\0';
    }
}
