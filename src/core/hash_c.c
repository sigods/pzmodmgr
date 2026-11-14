#include "core/hash_c.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rotate_right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

int pz_sha256_hex(const char* data, size_t length, pz_str* out) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static const char digits[] = "0123456789abcdef";
    uint64_t bit_length;
    size_t padded;
    size_t offset;
    size_t i;
    unsigned char* message;
    char hex[64];
    size_t at = 0;
    int ok;

    if (!out || (!data && length != 0)) return 0;
    if (length > UINT64_MAX / 8) return 0;
    if (length > SIZE_MAX - 72) return 0;

    bit_length = (uint64_t)length * 8;
    padded = length + 9;
    padded = ((padded + 63) / 64) * 64;

    message = (unsigned char*)malloc(padded);
    if (!message) return 0;
    if (length) memcpy(message, data, length);
    message[length] = 0x80;
    memset(message + length + 1, 0, padded - length - 1);
    for (i = 0; i < 8; ++i)
        message[padded - 1 - i] = (unsigned char)(bit_length >> (8 * i));

    for (offset = 0; offset < padded; offset += 64) {
        uint32_t w[64];
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
        uint32_t e;
        uint32_t f;
        uint32_t g;
        uint32_t hh;
        int round;

        for (i = 0; i < 16; ++i) {
            const size_t base = offset + i * 4;
            w[i] = ((uint32_t)message[base] << 24) |
                   ((uint32_t)message[base + 1] << 16) |
                   ((uint32_t)message[base + 2] << 8) |
                   (uint32_t)message[base + 3];
        }
        for (i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate_right(w[i - 15], 7) ^
                                rotate_right(w[i - 15], 18) ^
                                (w[i - 15] >> 3);
            const uint32_t s1 = rotate_right(w[i - 2], 17) ^
                                rotate_right(w[i - 2], 19) ^
                                (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        a = h[0];
        b = h[1];
        c = h[2];
        d = h[3];
        e = h[4];
        f = h[5];
        g = h[6];
        hh = h[7];
        for (round = 0; round < 64; ++round) {
            const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                                  rotate_right(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t temp1 = hh + sum1 + choose + k[round] + w[round];
            const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                                  rotate_right(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    free(message);

    for (i = 0; i < 8; ++i) {
        int shift;
        for (shift = 28; shift >= 0; shift -= 4)
            hex[at++] = digits[(h[i] >> shift) & 0x0f];
    }
    ok = pz_str_assign(out, hex, 64);
    return ok;
}
